/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: instruction execution and the SPLOOP loop buffer.
 */
#include "c66x_priv.h"

/* ------------------------------------------------------------------------ */
/* arithmetic helpers                                                        */

static inline int64_t sext40(uint64_t v) { return (int64_t)(v << 24) >> 24; }
static inline int32_t s16(uint32_t v) { return (int16_t)v; }
static inline int32_t lsb16s(uint32_t v) { return (int16_t)v; }
static inline int32_t msb16s(uint32_t v) { return (int16_t)(v >> 16); }
static inline uint32_t lsb16u(uint32_t v) { return v & 0xffff; }
static inline uint32_t msb16u(uint32_t v) { return v >> 16; }

static void set_sat(c66x_core *c, const c66x_insn *in)
{
    c->cr[CR_CSR] |= CSR_SAT;
    static const int8_t ssr_bit[8] = { 0, 1, 2, 3, -1, -1, 4, 5 };
    if (in->unit >= 0 && ssr_bit[in->unit] >= 0)
        c->cr[CR_SSR] |= 1u << ssr_bit[in->unit];
}

static int64_t sat_to(c66x_core *c, const c66x_insn *in, int64_t v, int bits)
{
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
    if (v > hi) { set_sat(c, in); return hi; }
    if (v < lo) { set_sat(c, in); return lo; }
    return v;
}

static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

static unsigned rmode(c66x_core *c, const c66x_insn *in)
{
    uint32_t r = (in->unit >= 6) ? c->cr[CR_FMCR] : c->cr[CR_FADCR];
    return in->side == 2 ? (r >> 25) & 3 : (r >> 9) & 3;
}

static const int fe_mode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

static inline float  u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline double u2d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }

static int32_t f_to_i32(double x, int mode_trunc, int rm)
{
    if (x != x || x >= 2147483648.0 || x < -2147483648.0)
        return (int32_t)0x80000000;
    if (mode_trunc)
        return (int32_t)x;
    double r;
    switch (rm) {
    case 1: r = trunc(x); break;
    case 2: r = ceil(x); break;
    case 3: r = floor(x); break;
    default: r = nearbyint(x); break;
    }
    if (r >= 2147483648.0 || r < -2147483648.0)
        return (int32_t)0x80000000;
    return (int32_t)r;
}

static uint32_t bitrev32(uint32_t v)
{
    v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
    v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
    v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
    v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
    return (v >> 16) | (v << 16);
}

static uint32_t gf_mul(uint32_t a, uint32_t b, uint32_t poly, unsigned size)
{
    uint32_t r = 0, top = 1u << (size - 1), mask = (size == 32) ? 0xffffffff : ((1u << size) - 1);
    while (b) {
        if (b & 1)
            r ^= a;
        b >>= 1;
        int carry = (a & top) != 0;
        a = (a << 1) & mask;
        if (carry)
            a ^= poly & mask;
    }
    return r;
}

/* ------------------------------------------------------------------------ */
/* execution                                                                 */


static uint64_t opval(c66x_core *c, const c66x_operand *o, uint32_t pce1)
{
    switch (o->kind) {
    case C66X_OPK_REG:   return c->reg[o->reg];
    case C66X_OPK_PAIR:
        if (o->size == 5)
            return ((uint64_t)(c->reg[o->reg_hi] & 0xff) << 32) | c->reg[o->reg];
        return ((uint64_t)c->reg[o->reg_hi] << 32) | c->reg[o->reg];
    case C66X_OPK_CONST: return (uint64_t)(int64_t)o->val;
    case C66X_OPK_ADDR:  return (uint32_t)o->val;
    case C66X_OPK_CTRL:  return ctrl_read(c, c66x_ctrl_crlo(o->val), pce1);
    case C66X_OPK_ILC:   return c->cr[CR_ILC];
    case C66X_OPK_IRP:   return c->cr[CR_IRP];
    case C66X_OPK_NRP:   return c->cr[CR_NRP];
    default: return 0;
    }
}

static inline int64_t sv(const c66x_operand *o, uint64_t v)
{
    if (o->kind == C66X_OPK_CONST)
        return (int64_t)v;
    if (o->size == 5)
        return sext40(v);
    if (o->size == 8)
        return (int64_t)v;
    return (int32_t)v;
}

static inline uint64_t uv(const c66x_operand *o, uint64_t v)
{
    if (o->size == 5)
        return v & 0xffffffffffULL;
    if (o->size == 8)
        return v;
    return (uint32_t)v;
}

static inline void write_op(c66x_core *c, const c66x_operand *o, uint64_t v, uint8_t load)
{
    switch (o->kind) {
    case C66X_OPK_REG:
        sched(c, o->low_first - 1, WK_REG, o->reg, (uint32_t)v, load);
        break;
    case C66X_OPK_PAIR:
        sched(c, o->low_first - 1, WK_REG, o->reg, (uint32_t)v, load);
        if (o->size == 5)
            sched(c, o->high_first - 1, WK_REG, o->reg_hi, (uint32_t)(v >> 32) & 0xff, load);
        else
            sched(c, o->high_first - 1, WK_REG, o->reg_hi, (uint32_t)(v >> 32), load);
        break;
    case C66X_OPK_CTRL: {
        unsigned crlo = c66x_ctrl_crlo(o->val);
        /* ILC/RILC take 4 cycles to reach the loop buffer (SPRU732 7.4.3). */
        unsigned d = (crlo == CR_ILC || crlo == CR_RILC) ? 3 : (crlo == CR_IFR || crlo == CR_ICR) ? 1 : 0;
        sched(c, d, WK_CTRL, crlo, (uint32_t)v, 0);
        break;
    }
    case C66X_OPK_ILC:
        sched(c, 3, WK_CTRL, CR_ILC, (uint32_t)v, 0);
        break;
    default:
        break;
    }
}

static uint32_t mem_ea(c66x_core *c, const c66x_operand *o)
{
    uint32_t base = c->reg[o->reg];
    uint32_t off = (o->mem_mode & 4) ? c->reg[o->mem_offreg] * o->mem_scale
                                     : (uint32_t)o->val * o->mem_scale;
    switch (o->mem_mode & ~4u) {
    case 0:  return base - off;
    case 1:  return base + off;
    case 8:  sched(c, 0, WK_REG, o->reg, base - off, 0); return base - off;
    case 9:  sched(c, 0, WK_REG, o->reg, base + off, 0); return base + off;
    case 10: sched(c, 0, WK_REG, o->reg, base - off, 0); return base;
    case 11: sched(c, 0, WK_REG, o->reg, base + off, 0); return base;
    default: return base;
    }
}

/* Branches taken in the delay slots of other branches all stay in flight. TI's
 * divide (stage 1, 0x00800280) seeds five in consecutive packets so its
 * one-packet loop at 0x0080030C has a branch landing every cycle. */
static inline int take_branch_rc(c66x_core *c, uint32_t target)
{
    if (c->nbr < NBR) {
        c->br[c->nbr++] = (pend_branch){ 6, target };
        return 0;
    }
    c->trap_pc = c->exec_pc;
    return C66X_STOP_FAULT;
}

static void take_branch(c66x_core *c, uint32_t target, xctx *x)
{
    int r = take_branch_rc(c, target);
    if (r)
        x->stop = r;
}

static void spl_start(c66x_core *c, const c66x_insn *in, xctx *x);

static inline void do_load(c66x_core *c, const c66x_insn *in)
{
    uint32_t ea = mem_ea(c, &in->op[0]);
    uint64_t val;
    switch (in->sub) {
    case LD_B:  val = (uint32_t)(int8_t)mem_read(c, ea, 1); break;
    case LD_BU: val = mem_read(c, ea, 1); break;
    case LD_H:  val = (uint32_t)(int16_t)mem_read(c, ea, 2); break;
    case LD_HU: val = mem_read(c, ea, 2); break;
    case LD_DW: val = mem_read(c, ea, 4) | ((uint64_t)mem_read(c, ea + 4, 4) << 32); break;
    default:    val = mem_read(c, ea, 4); break;
    }
    write_op(c, &in->op[1], val, 1);
}

static inline void do_store(c66x_core *c, const c66x_insn *in, uint32_t pce1)
{
    uint64_t val = opval(c, &in->op[0], pce1);
    uint32_t ea = mem_ea(c, &in->op[1]);
    if (in->sub == LD_B) store_defer(c, ea, (uint32_t)val, 1);
    else if (in->sub == LD_H) store_defer(c, ea, (uint32_t)val, 2);
    else if (in->sub == LD_DW) {
        store_defer(c, ea, (uint32_t)val, 4);
        store_defer(c, ea + 4, (uint32_t)(val >> 32), 4);
    } else store_defer(c, ea, (uint32_t)val, 4);
}

/* exec_insn for the F_* forms, with the operand decoding resolved at classify. */
/* The effect of one fast-form instruction, without the packet bookkeeping
 * (NOP cycles, branch flag) that a cached packet carries precomputed. Returns a
 * stop code, 0 normally. */
/* F_STORE's inline RAM store for one 32-bit word, else store_defer. */
static inline void store_word_fast(c66x_core *c, uint32_t ea, uint32_t v)
{
    ramreg *rr = &c->ram[c->last_ram];
    if (c->store_now && !c->watch_fn && (!c->idle_armed || c->idle_fx)
        && ea - rr->base <= rr->size - 4 && rr->size >= 4) {
        uint8_t *m = rr->host + (ea - rr->base);
        m[0] = v; m[1] = v >> 8; m[2] = v >> 16; m[3] = v >> 24;
        if (rr->codepage[(ea - rr->base) >> FP_PAGE_SHIFT])
            invalidate_code(c, ea, 4);
        return;
    }
    store_defer(c, ea, v, 4);
}

int fop_run(c66x_core *c, const c66x_insn *in, uint32_t next_pc)
{
    const c66x_operand *op = in->op;
    HC(fop, in->fop);
    if (in->fop == F_NOP)
        return 0;
    if (in->cond_reg >= 0 && ((c->reg[in->cond_reg] == 0) != in->cond_z))
        return 0;
    switch (in->fop) {
    case F_LOAD:
        if (in->mfast) {
            uint32_t base = c->reg[op[0].reg], ea = base + (uint32_t)in->ea_delta, v;
            unsigned n = in->mfast & 7;
            if (in->mfast & 0x30) {
                /* mem_ea's order: the base update is scheduled before the read */
                if (c->imm_n < IMM_DEPTH)
                    c->imm[c->imm_n++] = (wq_ent){ WK_REG, op[0].reg, 0, ea };
                if (in->mfast & 0x20)
                    ea = base;
            }
            ramreg *rr = &c->ram[c->last_ram];
            /* mem_read's RAM path, minus the idle tracker when it would act */
            if ((!c->idle_armed || c->isr_depth || c->idle_fx)
                && ea - rr->base <= rr->size - n && rr->size >= n) {
                const uint8_t *m = rr->host + (ea - rr->base);
                v = n == 4 ? m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t)m[3] << 24)
                  : n == 2 ? (uint32_t)(m[0] | (m[1] << 8)) : m[0];
            } else {
                v = mem_read(c, ea, n);
            }
            if (in->mfast & 8)
                v = n == 1 ? (uint32_t)(int8_t)v : (uint32_t)(int16_t)v;
            sched(c, op[1].low_first - 1, WK_REG, op[1].reg, v, 1);
            return 0;
        }
        do_load(c, in);
        return 0;
    case F_STORE:
        if (in->mfast) {
            uint32_t base = c->reg[op[1].reg], ea = base + (uint32_t)in->ea_delta, v = c->reg[op[0].reg];
            unsigned n = in->mfast & 7;
            if (in->mfast & 0x30) {
                if (c->imm_n < IMM_DEPTH)
                    c->imm[c->imm_n++] = (wq_ent){ WK_REG, op[1].reg, 0, ea };
                if (in->mfast & 0x20)
                    ea = base;
            }
            if (in->mfast & 0x40) {
                /* stdw: low word at ea, high word at ea + 4 */
                uint32_t hi = c->reg[op[0].reg_hi];
                store_word_fast(c, ea, v);
                store_word_fast(c, ea + 4, hi);
                return 0;
            }
            ramreg *rr = &c->ram[c->last_ram];
            /* mem_write's RAM path when nothing else would look at the store */
            if (c->store_now && !c->watch_fn && (!c->idle_armed || c->idle_fx)
                && ea - rr->base <= rr->size - n && rr->size >= n) {
                uint8_t *m = rr->host + (ea - rr->base);
                m[0] = v;
                if (n > 1) m[1] = v >> 8;
                if (n > 2) { m[2] = v >> 16; m[3] = v >> 24; }
                if (rr->codepage[(ea - rr->base) >> FP_PAGE_SHIFT])
                    invalidate_code(c, ea, n);
                return 0;
            }
            store_defer(c, ea, v, n);
            return 0;
        }
        do_store(c, in, in->addr & ~31u);
        return 0;
    case F_BRANCH:
        return take_branch_rc(c, (uint32_t)opval(c, &op[0], in->addr & ~31u));
    case F_CALLP: {
        int r = take_branch_rc(c, (uint32_t)op[0].val);
        write_op(c, &op[1], next_pc, 0);
        return r;
    }
    case F_MVC:
        write_op(c, &op[1], opval(c, &op[0], in->addr & ~31u), 0);
        return 0;
    case F_ADDSP: case F_SUBSP: case F_MPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float p = u2f(c->reg[op[0].reg]), q = u2f(c->reg[op[1].reg]);
        float res = in->fop == F_ADDSP ? p + q : in->fop == F_SUBSP ? p - q : p * q;
        if (rm) fesetround(FE_TONEAREST);
        sched(c, op[2].low_first - 1, WK_REG, op[2].reg, f2u(res), 0);
        return 0;
    }
    default:
        break;
    }
    uint32_t a = op[0].kind == C66X_OPK_REG ? c->reg[op[0].reg] : (uint32_t)op[0].val;
    uint32_t b = op[1].kind == C66X_OPK_REG ? c->reg[op[1].reg] : (uint32_t)op[1].val;
    int32_t sa = op[0].kind == C66X_OPK_REG ? (int32_t)a : op[0].val;
    int32_t sb = op[1].kind == C66X_OPK_REG ? (int32_t)b : op[1].val;
    uint32_t r;
    unsigned dst = 2;
    switch (in->fop) {
    case F_MVK:    r = (uint32_t)op[0].val; dst = 1; break;
    case F_MVKH:   r = (b & 0xffff) | ((uint32_t)op[0].val & 0xffff0000); dst = 1; break;
    case F_ADDK:   r = b + (uint32_t)op[0].val; dst = 1; break;
    case F_MV:     r = a; dst = 1; break;
    case F_ADDKPC: r = (uint32_t)op[0].val; dst = 1; break;
    case F_ADD:    r = a + b; break;
    case F_SUB:    r = a - b; break;
    case F_AND:    r = a & b; break;
    case F_OR:     r = a | b; break;
    case F_XOR:    r = a ^ b; break;
    case F_CMPEQ:  r = sa == sb; break;
    case F_CMPGT:  r = sa > sb; break;
    case F_CMPLT:  r = sa < sb; break;
    case F_CMPGTU: r = a > b; break;
    case F_CMPLTU: r = a < b; break;
    case F_SHL:    r = (b & 0x3f) > 31 ? 0 : a << (b & 0x3f); break;
    case F_SHR:    r = (uint32_t)(sa >> ((b & 0x3f) > 31 ? 31 : (b & 0x3f))); break;
    case F_SHRU:   r = (b & 0x3f) > 31 ? 0 : a >> (b & 0x3f); break;
    case F_EXT:    r = (uint32_t)((int32_t)(a << (op[1].val & 31)) >> (op[2].val & 31)); dst = 3; break;
    case F_EXTU:   r = (a << (op[1].val & 31)) >> (op[2].val & 31); dst = 3; break;
    default:       return 0;
    }
    if (c->imm_n < IMM_DEPTH)
        c->imm[c->imm_n++] = (wq_ent){ WK_REG, op[dst].reg, 0, r };
    return 0;
}


/* One instruction. Reads happen now (E1); writes are scheduled at the cycles
 * the binutils operand table gives. */
void exec_insn(c66x_core *c, c66x_insn *in, xctx *x)
{
    unsigned h = in->handler;
    const c66x_operand *op = in->op;
    uint64_t v[4] = { 0 };
    int64_t r;
    uint32_t a, b;

    c->st.insns++;
    HC(handler, h);

    if (in->xnops > x->extra_nops)
        x->extra_nops = in->xnops;
    x->branched |= in->isbranch;
    if (h == H_NOP)
        return;

    if (in->cond_reg >= 0 && ((c->reg[in->cond_reg] == 0) != in->cond_z)) {
        /* An SPLOOP family predicate is an exit or reload test, not an enable. */
        if (h == H_SPLOOPW || h == H_SPLOOP || h == H_SPLOOPD)
            spl_start(c, in, x);
        return;
    }

    for (unsigned m = in->rmask; m; m &= m - 1) {
        unsigned i = __builtin_ctz(m);
        v[i] = (in->regmask >> i) & 1 ? c->reg[op[i].reg]
             : (in->constmask >> i) & 1 ? (uint64_t)(int64_t)op[i].val
             : opval(c, &op[i], x->pce1);
    }

#define SV(i) sv(&op[i], v[i])
#define UV(i) uv(&op[i], v[i])
#define W(i, val) write_op(c, &op[i], (uint64_t)(val), 0)
#define LAST (in->nops - 1)

    switch (h) {
    case H_UNIMP:
        c->trap_pc = in->addr;
        x->stop = C66X_STOP_UNDEF;
        return;
    case H_IDLE:
        c->idle = 1;
        return;
    case H_DINT:
        /* DINT/RINT move GIE to/from SGIE (SPRU732 DINT description). */
        c->cr[CR_TSR] = (c->cr[CR_TSR] & ~(TSR_GIE | TSR_SGIE))
                        | ((c->cr[CR_TSR] & TSR_GIE) ? TSR_SGIE : 0);
        return;
    case H_RINT:
        c->cr[CR_TSR] = (c->cr[CR_TSR] & ~TSR_GIE)
                        | ((c->cr[CR_TSR] & TSR_SGIE) ? TSR_GIE : 0);
        return;
    case H_SWE:
        c->trap_pc = in->addr;
        x->stop = C66X_STOP_FAULT;
        return;

    case H_ABS: r = SV(0); W(1, r < 0 ? -r : r); return;
    case H_ABS2:
        a = (uint16_t)abs(lsb16s(v[0])); b = (uint16_t)abs(msb16s(v[0]));
        W(1, a | (b << 16)); return;
    case H_ADD: W(LAST, SV(0) + SV(1)); return;
    case H_ADDU: W(LAST, UV(0) + UV(1)); return;
    case H_SUB: W(LAST, SV(0) - SV(1)); return;
    case H_SUBU: W(LAST, UV(0) - UV(1)); return;
    case H_ADD2:
        W(2, ((uint32_t)(v[0] + v[1]) & 0xffff) | ((((uint32_t)v[0] >> 16) + ((uint32_t)v[1] >> 16)) << 16));
        return;
    case H_SUB2:
        W(2, ((uint32_t)(v[0] - v[1]) & 0xffff) | ((((uint32_t)v[0] >> 16) - ((uint32_t)v[1] >> 16)) << 16));
        return;
    case H_ADD4: case H_SUB4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            uint32_t p = (uint32_t)v[0] >> k & 0xff, q = (uint32_t)v[1] >> k & 0xff;
            o |= ((h == H_ADD4 ? p + q : p - q) & 0xff) << k;
        }
        W(2, o);
        return;
    }
    case H_ADDA: case H_SUBA: {
        unsigned sh = in->sub;
        uint32_t base = (uint32_t)v[0];
        uint32_t off = (uint32_t)v[1];
        W(LAST, h == H_ADDA ? base + (off << sh) : base - (off << sh));
        return;
    }
    case H_ADDK: W(1, (uint32_t)c->reg[op[1].reg] + (int32_t)op[0].val); return;
    case H_ADDKPC: W(1, (uint32_t)op[0].val); return;
    case H_ADDSUB: case H_SADDSUB: {
        int64_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        int64_t ad = s1 + s2, sb = s1 - s2;
        if (h == H_SADDSUB) { ad = sat_to(c, in, ad, 32); sb = sat_to(c, in, sb, 32); }
        W(2, ((uint64_t)(uint32_t)ad << 32) | (uint32_t)sb);
        return;
    }
    case H_ADDSUB2: case H_SADDSUB2: {
        int32_t p1 = lsb16s(v[0]), p2 = msb16s(v[0]), q1 = lsb16s(v[1]), q2 = msb16s(v[1]);
        int32_t al = p1 + q1, ah = p2 + q2, sl = p1 - q1, sh2 = p2 - q2;
        if (h == H_SADDSUB2) { al = sat16(al); ah = sat16(ah); sl = sat16(sl); sh2 = sat16(sh2); }
        uint32_t o = ((uint16_t)ah << 16) | (uint16_t)al;
        uint32_t e = ((uint16_t)sh2 << 16) | (uint16_t)sl;
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_AND: case H_ANDN: case H_OR: case H_XOR: {
        uint64_t p = v[0], q = v[1];
        /* C66x pair forms apply a scst5 to both 32-bit halves. */
        if (op[0].kind == C66X_OPK_CONST && op[2].size == 8)
            p = ((uint64_t)(uint32_t)op[0].val << 32) | (uint32_t)op[0].val;
        W(2, h == H_AND ? p & q : h == H_ANDN ? p & ~q : h == H_OR ? p | q : p ^ q);
        return;
    }
    case H_NOT: W(1, ~v[0]); return;
    case H_NEG: W(1, -SV(0)); return;
    case H_MV: W(1, v[0]); return;
    case H_MVK: W(1, (int64_t)op[0].val); return;
    case H_MVKH:
        W(1, (c->reg[op[1].reg] & 0xffff) | ((uint32_t)op[0].val & 0xffff0000));
        return;
    case H_ZERO: W(0, 0); return;
    case H_DMV: W(2, ((uint64_t)(uint32_t)v[0] << 32) | (uint32_t)v[1]); return;
    /* binutils marks the compact forms' source (ORREG1B, ORREG1BNORS) as
     * written, so read those operands here rather than trusting rw. */
    case H_MVC: W(1, opval(c, &op[0], x->pce1)); return;
    case H_MVD: W(1, v[0]); return;

    case H_CMPEQ: W(2, SV(0) == SV(1)); return;
    case H_CMPGT: W(2, SV(0) > SV(1)); return;
    case H_CMPLT: W(2, SV(0) < SV(1)); return;
    case H_CMPGTU:
        W(2, (op[0].kind == C66X_OPK_CONST ? (uint64_t)(uint32_t)op[0].val : UV(0)) > UV(1));
        return;
    case H_CMPLTU:
        W(2, (op[0].kind == C66X_OPK_CONST ? (uint64_t)(uint32_t)op[0].val : UV(0)) < UV(1));
        return;
    case H_CMPEQ2: case H_CMPGT2: case H_CMPLT2: {
        int32_t l1 = lsb16s(v[0]), h1 = msb16s(v[0]), l2 = lsb16s(v[1]), h2 = msb16s(v[1]);
        unsigned d0, d1;
        if (h == H_CMPEQ2) { d0 = l1 == l2; d1 = h1 == h2; }
        else if (h == H_CMPGT2) { d0 = l1 > l2; d1 = h1 > h2; }
        else { d0 = l1 < l2; d1 = h1 < h2; }
        W(2, d0 | (d1 << 1));
        return;
    }
    case H_CMPEQ4: case H_CMPGTU4: case H_CMPLTU4: {
        uint32_t o = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t p = (uint32_t)v[0] >> (8 * k), q = (uint32_t)v[1] >> (8 * k);
            unsigned t = h == H_CMPEQ4 ? (int8_t)p == (int8_t)q : h == H_CMPGTU4 ? p > q : p < q;
            o |= t << k;
        }
        W(2, o);
        return;
    }

    case H_SHL: {
        unsigned n = (uint32_t)v[1] & 0x3f;
        uint64_t s = UV(0);
        uint64_t res = n > 39 ? 0 : s << n;
        W(2, res);
        return;
    }
    case H_SHR: {
        unsigned n = (uint32_t)v[1] & 0x3f;
        int64_t s = SV(0);
        if (n > 39) n = 39;
        W(2, s >> n);
        return;
    }
    case H_SHRU: {
        unsigned n = (uint32_t)v[1] & 0x3f;
        uint64_t s = UV(0);
        W(2, n > 39 ? 0 : s >> n);
        return;
    }
    case H_SSHL: {
        unsigned n = (uint32_t)v[1] & 0x1f;
        W(2, sat_to(c, in, (int64_t)(int32_t)v[0] << n, 32));
        return;
    }
    case H_SHR2: case H_SHRU2: {
        unsigned n = (uint32_t)v[1] & 0x1f;
        uint32_t o;
        if (h == H_SHR2) {
            int32_t l = lsb16s(v[0]), m = msb16s(v[0]);
            if (n > 15) n = 15;
            o = ((uint16_t)(l >> n)) | ((uint16_t)(m >> n) << 16);
        } else {
            uint32_t l = lsb16u(v[0]), m = msb16u(v[0]);
            o = n > 15 ? 0 : ((l >> n) & 0xffff) | (((m >> n) & 0xffff) << 16);
        }
        W(2, o);
        return;
    }
    case H_SSHVL: case H_SSHVR: {
        /* src2 (op0) shifted by the signed 6-bit amount in src1 (op1). */
        int32_t s = (int32_t)v[0];
        int32_t n = (int32_t)((uint32_t)v[1] << 26) >> 26;
        if (h == H_SSHVR) n = -n;
        int64_t res;
        if (n >= 0) {
            res = n >= 32 ? (s ? (s > 0 ? INT64_MAX : INT64_MIN) : 0) : (int64_t)s << n;
            res = sat_to(c, in, res, 32);
        } else {
            res = s >> (-n > 31 ? 31 : -n);
        }
        W(2, res);
        return;
    }
    case H_SHLMB: W(2, ((uint32_t)v[1] << 8) | ((uint32_t)v[0] >> 24)); return;
    case H_SHRMB: W(2, ((uint32_t)v[1] >> 8) | (((uint32_t)v[0] & 0xff) << 24)); return;
    case H_ROTL: {
        unsigned n = (uint32_t)v[1] & 31;
        uint32_t s = (uint32_t)v[0];
        W(2, n ? (s << n) | (s >> (32 - n)) : s);
        return;
    }
    case H_CLR: case H_SET: case H_EXT: case H_EXTU: {
        unsigned csta, cstb;
        uint32_t s = (uint32_t)v[0];
        if (in->nops == 4) {
            csta = op[1].val & 31; cstb = op[2].val & 31;
        } else {
            /* register form: src1 (op1) holds csta in 9..5 and cstb in 4..0 */
            s = (uint32_t)v[0];
            csta = ((uint32_t)v[1] >> 5) & 31; cstb = (uint32_t)v[1] & 31;
        }
        uint32_t res;
        if (h == H_CLR || h == H_SET) {
            if (cstb < csta) { res = s; }
            else {
                unsigned w = cstb - csta + 1;
                uint32_t m = (w >= 32 ? 0xffffffff : ((1u << w) - 1)) << csta;
                res = h == H_CLR ? s & ~m : s | m;
            }
        } else {
            uint32_t t = s << csta;
            res = h == H_EXT ? (uint32_t)((int32_t)t >> cstb) : (t >> cstb);
        }
        W(LAST, res);
        return;
    }

    case H_SADD: {
        int bits = op[LAST].size == 5 ? 40 : 32;
        W(2, sat_to(c, in, SV(0) + SV(1), bits));
        return;
    }
    case H_SSUB: {
        int bits = op[LAST].size == 5 ? 40 : 32;
        W(2, sat_to(c, in, SV(0) - SV(1), bits));
        return;
    }
    case H_SAT: W(1, sat_to(c, in, SV(0), 32)); return;
    case H_SADD2: case H_SSUB2: {
        int32_t l = h == H_SADD2 ? lsb16s(v[0]) + lsb16s(v[1]) : lsb16s(v[0]) - lsb16s(v[1]);
        int32_t m = h == H_SADD2 ? msb16s(v[0]) + msb16s(v[1]) : msb16s(v[0]) - msb16s(v[1]);
        if (l != sat16(l) || m != sat16(m)) set_sat(c, in);
        W(2, (uint16_t)sat16(l) | ((uint32_t)(uint16_t)sat16(m) << 16));
        return;
    }
    case H_SADDUS2: case H_SADDSU2: {
        /* unsigned 16-bit fields of one operand plus signed fields of the other, saturated to u16 */
        uint32_t us = h == H_SADDUS2 ? (uint32_t)v[0] : (uint32_t)v[1];
        uint32_t ss = h == H_SADDUS2 ? (uint32_t)v[1] : (uint32_t)v[0];
        int32_t l = (int32_t)lsb16u(us) + lsb16s(ss), m = (int32_t)msb16u(us) + msb16s(ss);
        l = l < 0 ? 0 : l > 65535 ? 65535 : l;
        m = m < 0 ? 0 : m > 65535 ? 65535 : m;
        W(2, (uint32_t)l | ((uint32_t)m << 16));
        return;
    }
    case H_SADDU4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            unsigned s = (((uint32_t)v[0] >> k) & 0xff) + (((uint32_t)v[1] >> k) & 0xff);
            o |= (s > 255 ? 255 : s) << k;
        }
        W(2, o);
        return;
    }
    case H_SUBC: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        W(2, s1 >= s2 ? ((s1 - s2) << 1) + 1 : s1 << 1);
        return;
    }
    case H_SUBABS4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            int d = (int)(((uint32_t)v[0] >> k) & 0xff) - (int)(((uint32_t)v[1] >> k) & 0xff);
            o |= (uint32_t)(d < 0 ? -d : d) << k;
        }
        W(2, o);
        return;
    }
    case H_NORM: {
        int bits = op[0].size == 5 ? 40 : 32;
        int64_t s = SV(0);
        uint64_t t = (uint64_t)(s < 0 ? ~s : s);
        int n = 0;
        for (int k = bits - 2; k >= 0 && !((t >> k) & 1); k--)
            n++;
        W(1, n);
        return;
    }
    case H_LMBD: {
        unsigned bit = (uint32_t)v[0] & 1;
        uint32_t s = (uint32_t)v[1];
        unsigned n = 0;
        while (n < 32 && (((s >> (31 - n)) & 1) != bit))
            n++;
        W(2, n);
        return;
    }
    case H_BITC4: {
        uint32_t o = 0, s = (uint32_t)v[0];
        for (int k = 0; k < 32; k += 8)
            o |= (uint32_t)__builtin_popcount((s >> k) & 0xff) << k;
        W(1, o);
        return;
    }
    case H_BITR: W(1, bitrev32((uint32_t)v[0])); return;
    case H_DEAL: {
        uint32_t s = (uint32_t)v[0], o = 0;
        for (int k = 0; k < 16; k++) {
            o |= ((s >> (2 * k + 1)) & 1) << (16 + k);
            o |= ((s >> (2 * k)) & 1) << k;
        }
        W(1, o);
        return;
    }
    case H_SHFL: {
        uint32_t s = (uint32_t)v[0], o = 0;
        for (int k = 0; k < 16; k++) {
            o |= ((s >> (16 + k)) & 1) << (2 * k + 1);
            o |= ((s >> k) & 1) << (2 * k);
        }
        W(1, o);
        return;
    }
    case H_XPND2: {
        uint32_t s = (uint32_t)v[0];
        W(1, (s & 1 ? 0x0000ffff : 0) | (s & 2 ? 0xffff0000 : 0));
        return;
    }
    case H_XPND4: {
        uint32_t s = (uint32_t)v[0], o = 0;
        for (int k = 0; k < 4; k++)
            if (s & (1u << k)) o |= 0xffu << (8 * k);
        W(1, o);
        return;
    }
    case H_SWAP2: { uint32_t s = (uint32_t)v[0]; W(1, (s << 16) | (s >> 16)); return; }
    case H_SWAP4: {
        uint32_t s = (uint32_t)v[0];
        W(1, ((s & 0x00ff00ff) << 8) | ((s >> 8) & 0x00ff00ff));
        return;
    }
    case H_AVG2: {
        int32_t l = (lsb16s(v[0]) + lsb16s(v[1]) + 1) >> 1, m = (msb16s(v[0]) + msb16s(v[1]) + 1) >> 1;
        W(2, (uint16_t)l | ((uint32_t)(uint16_t)m << 16));
        return;
    }
    case H_AVGU4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8)
            o |= (((((uint32_t)v[0] >> k) & 0xff) + (((uint32_t)v[1] >> k) & 0xff) + 1) >> 1) << k;
        W(2, o);
        return;
    }
    case H_MAX2: case H_MIN2: {
        int32_t l1 = lsb16s(v[0]), l2 = lsb16s(v[1]), m1 = msb16s(v[0]), m2 = msb16s(v[1]);
        int32_t l = h == H_MAX2 ? (l1 >= l2 ? l1 : l2) : (l1 <= l2 ? l1 : l2);
        int32_t m = h == H_MAX2 ? (m1 >= m2 ? m1 : m2) : (m1 <= m2 ? m1 : m2);
        W(2, (uint16_t)l | ((uint32_t)(uint16_t)m << 16));
        return;
    }
    case H_MAXU4: case H_MINU4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            uint32_t p = ((uint32_t)v[0] >> k) & 0xff, q = ((uint32_t)v[1] >> k) & 0xff;
            o |= (h == H_MAXU4 ? (p >= q ? p : q) : (p <= q ? p : q)) << k;
        }
        W(2, o);
        return;
    }
    case H_PACK2:   W(2, (((uint32_t)v[0] & 0xffff) << 16) | ((uint32_t)v[1] & 0xffff)); return;
    case H_PACKH2:  W(2, (((uint32_t)v[0] >> 16) << 16) | ((uint32_t)v[1] >> 16)); return;
    case H_PACKHL2: W(2, (((uint32_t)v[0] >> 16) << 16) | ((uint32_t)v[1] & 0xffff)); return;
    case H_PACKLH2: W(2, (((uint32_t)v[0] & 0xffff) << 16) | ((uint32_t)v[1] >> 16)); return;
    case H_PACKH4: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        W(2, ((s1 >> 24) << 24) | (((s1 >> 8) & 0xff) << 16) | ((s2 >> 24) << 8) | ((s2 >> 8) & 0xff));
        return;
    }
    case H_PACKL4: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        W(2, (((s1 >> 16) & 0xff) << 24) | ((s1 & 0xff) << 16) | (((s2 >> 16) & 0xff) << 8) | (s2 & 0xff));
        return;
    }
    case H_SPACK2: {
        int32_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        int32_t a1 = s1 > 32767 ? 32767 : s1 < -32768 ? -32768 : s1;
        int32_t a2 = s2 > 32767 ? 32767 : s2 < -32768 ? -32768 : s2;
        W(2, ((uint32_t)(uint16_t)a1 << 16) | (uint16_t)a2);
        return;
    }
    case H_SPACKU4: {
        uint32_t o = 0;
        int16_t f[4] = { msb16s(v[0]), lsb16s(v[0]), msb16s(v[1]), lsb16s(v[1]) };
        for (int k = 0; k < 4; k++) {
            int t = f[k] > 255 ? 255 : f[k] < 0 ? 0 : f[k];
            o |= (uint32_t)t << (8 * (3 - k));
        }
        W(2, o);
        return;
    }
    case H_RPACK2: {
        int32_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        int32_t a1 = (int32_t)sat_to(c, in, (int64_t)s1 << 1, 32) + 0x8000;
        int32_t a2 = (int32_t)sat_to(c, in, (int64_t)s2 << 1, 32) + 0x8000;
        W(2, (((uint32_t)a1 >> 16) << 16) | ((uint32_t)a2 >> 16));
        return;
    }
    case H_DPACK2: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        uint32_t e = ((s1 & 0xffff) << 16) | (s2 & 0xffff);
        uint32_t o = ((s1 >> 16) << 16) | (s2 >> 16);
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_DPACKX2: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        uint32_t e = ((s1 & 0xffff) << 16) | (s2 >> 16);
        uint32_t o = ((s2 & 0xffff) << 16) | (s1 >> 16);
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_UNPKHU4: {
        uint32_t s = (uint32_t)v[0];
        W(1, (((s >> 24) & 0xff) << 16) | ((s >> 16) & 0xff));
        return;
    }
    case H_UNPKLU4: {
        uint32_t s = (uint32_t)v[0];
        W(1, (((s >> 8) & 0xff) << 16) | (s & 0xff));
        return;
    }

    case H_MPY16: case H_SMPY16: {
        int f1h = in->sub & 1, f2h = (in->sub >> 1) & 1;
        char sg1 = (in->sub & 4) ? 'u' : 's', sg2 = (in->sub & 8) ? 'u' : 's';
        uint32_t x1 = (uint32_t)v[0], x2 = (uint32_t)v[1];
        if (op[0].kind == C66X_OPK_CONST) x1 = (uint32_t)op[0].val;
        int64_t a1 = f1h ? (sg1 == 'u' ? msb16u(x1) : msb16s(x1)) : (sg1 == 'u' ? lsb16u(x1) : lsb16s(x1));
        int64_t a2 = f2h ? (sg2 == 'u' ? msb16u(x2) : msb16s(x2)) : (sg2 == 'u' ? lsb16u(x2) : lsb16s(x2));
        int64_t prod = a1 * a2;
        if (h == H_SMPY16) {
            prod <<= 1;
            if (prod == 0x80000000LL) { set_sat(c, in); prod = 0x7fffffff; }
        }
        W(2, prod);
        return;
    }
    case H_MPY32: {
        int64_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        uint64_t u1 = (uint32_t)v[0], u2 = (uint32_t)v[1];
        uint64_t res;
        if (in->sub == 1) res = u1 * u2;
        else if (in->sub == 2) res = (uint64_t)(s1 * (int64_t)u2);
        else if (in->sub == 3) res = (uint64_t)((int64_t)u1 * s2);
        else res = (uint64_t)(s1 * s2);
        W(2, res);
        return;
    }
    case H_MPYI: case H_MPYID: {
        int64_t s1 = op[0].kind == C66X_OPK_CONST ? op[0].val : (int32_t)v[0];
        W(2, s1 * (int32_t)v[1]);
        return;
    }
    case H_MPYHI: case H_MPYHIR: case H_MPYLI: case H_MPYLIR: {
        /* 16-bit field of one operand times the 32-bit other; mpyih/mpyil list the 32-bit one first */
        int swapped = in->sub;
        uint32_t x16 = (uint32_t)(swapped ? v[1] : v[0]);
        int64_t x32 = (int32_t)(swapped ? v[0] : v[1]);
        int64_t f = (h == H_MPYHI || h == H_MPYHIR) ? msb16s(x16) : lsb16s(x16);
        int64_t prod = f * x32;
        if (h == H_MPYHIR || h == H_MPYLIR)
            W(2, (prod + 0x4000) >> 15);
        else
            W(2, prod);
        return;
    }
    case H_MPY2: case H_SMPY2: case H_MPYSU4: case H_MPYU4: case H_MPYUS4: {
        uint32_t x1 = (uint32_t)v[0], x2 = (uint32_t)v[1];
        int64_t e, o;
        if (h == H_MPY2 || h == H_SMPY2) {
            e = (int64_t)lsb16s(x1) * lsb16s(x2);
            o = (int64_t)msb16s(x1) * msb16s(x2);
            if (h == H_SMPY2) {
                e <<= 1; o <<= 1;
                if (e == 0x80000000LL) { e = 0x7fffffff; set_sat(c, in); }
                if (o == 0x80000000LL) { o = 0x7fffffff; set_sat(c, in); }
            }
        } else {
            /* four 8x8 products, packed 16 bits each into the pair */
            uint64_t res = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t p = (x1 >> (8 * k)) & 0xff, q = (x2 >> (8 * k)) & 0xff;
                int32_t t = h == H_MPYU4 ? (int32_t)(p * q)
                           : h == H_MPYSU4 ? (int8_t)p * (int32_t)q : (int32_t)p * (int8_t)q;
                res |= (uint64_t)(uint16_t)t << (16 * k);
            }
            W(2, res);
            return;
        }
        W(2, ((uint64_t)(uint32_t)o << 32) | (uint32_t)e);
        return;
    }
    case H_SMPY32: {
        int64_t p = (int64_t)(int32_t)v[0] * (int32_t)v[1];
        /* (src1 * src2 << 1) >> 32, saturating the single overflow case */
        if (p == 0x4000000000000000LL) { set_sat(c, in); W(2, 0x7fffffff); return; }
        W(2, (p << 1) >> 32);
        return;
    }
    case H_MPY2IR: {
        int64_t p1 = (int64_t)msb16s(v[0]) * (int32_t)v[1];
        int64_t p2 = (int64_t)lsb16s(v[0]) * (int32_t)v[1];
        W(2, ((uint64_t)(uint32_t)((p1 + 0x4000) >> 15) << 32) | (uint32_t)((p2 + 0x4000) >> 15));
        return;
    }
    case H_DOTP2: case H_DOTPN2: {
        int64_t lo = (int64_t)lsb16s(v[0]) * lsb16s(v[1]);
        int64_t hi = (int64_t)msb16s(v[0]) * msb16s(v[1]);
        int64_t res = h == H_DOTP2 ? lo + hi : hi - lo;
        W(2, res);
        return;
    }
    case H_DOTPRSU2: case H_DOTPNRSU2: {
        /* signed 16-bit fields of src1 x unsigned fields of src2; the "us" twins list src2 first */
        int us = in->sub;
        uint32_t s1 = (uint32_t)(us ? v[1] : v[0]), s2 = (uint32_t)(us ? v[0] : v[1]);
        int64_t hi = (int64_t)msb16s(s1) * msb16u(s2);
        int64_t lo = (int64_t)lsb16s(s1) * lsb16u(s2);
        int64_t t = (h == H_DOTPRSU2 ? hi + lo : hi - lo) + 0x8000;
        W(2, t >> 16);
        return;
    }
    case H_DOTPSU4: case H_DOTPU4: {
        int us = in->sub;
        uint32_t s1 = (uint32_t)(us ? v[1] : v[0]), s2 = (uint32_t)(us ? v[0] : v[1]);
        int64_t sum = 0;
        for (int k = 0; k < 32; k += 8) {
            uint32_t p = (s1 >> k) & 0xff, q = (s2 >> k) & 0xff;
            sum += h == H_DOTPU4 ? (int64_t)p * q : (int64_t)(int8_t)p * q;
        }
        W(2, sum);
        return;
    }
    case H_DDOTP4: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        int64_t e = (int64_t)msb16s(s1) * (int8_t)(s2 >> 8) + (int64_t)lsb16s(s1) * (int8_t)s2;
        int64_t o = (int64_t)msb16s(s1) * (int8_t)(s2 >> 24) + (int64_t)lsb16s(s1) * (int8_t)(s2 >> 16);
        W(2, ((uint64_t)(uint32_t)o << 32) | (uint32_t)e);
        return;
    }
    case H_DDOTPH2: case H_DDOTPL2: case H_DDOTPH2R: case H_DDOTPL2R: {
        uint32_t se = (uint32_t)v[0], so = (uint32_t)(v[0] >> 32), s2 = (uint32_t)v[1];
        int64_t first, second;
        if (h == H_DDOTPH2 || h == H_DDOTPH2R) {
            first = (int64_t)msb16s(so) * msb16s(s2) + (int64_t)lsb16s(so) * lsb16s(s2);
        } else {
            first = (int64_t)msb16s(se) * msb16s(s2) + (int64_t)lsb16s(se) * lsb16s(s2);
        }
        second = (int64_t)lsb16s(so) * msb16s(s2) + (int64_t)msb16s(se) * lsb16s(s2);
        if (h == H_DDOTPH2 || h == H_DDOTPL2) {
            int64_t f = sat_to(c, in, first, 32), s = sat_to(c, in, second, 32);
            W(2, ((uint64_t)(uint32_t)f << 32) | (uint32_t)s);
        } else {
            uint32_t f = (uint32_t)sat_to(c, in, first + 0x8000, 32) >> 16;
            uint32_t s = (uint32_t)sat_to(c, in, second + 0x8000, 32) >> 16;
            if (h == H_DDOTPH2R) W(2, (f << 16) | (s & 0xffff));
            else W(2, (s << 16) | (f & 0xffff));
        }
        return;
    }
    case H_CMPY: case H_CMPYR: case H_CMPYR1: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        int64_t e = (int64_t)lsb16s(s1) * msb16s(s2) + (int64_t)msb16s(s1) * lsb16s(s2);
        int64_t o = (int64_t)msb16s(s1) * msb16s(s2) - (int64_t)lsb16s(s1) * lsb16s(s2);
        e = sat_to(c, in, e, 32);
        if (h == H_CMPY) {
            W(2, ((uint64_t)(uint32_t)o << 32) | (uint32_t)e);
        } else if (h == H_CMPYR) {
            o = sat_to(c, in, o, 32);
            uint32_t lo = (uint32_t)sat_to(c, in, e + 0x8000, 32) >> 16;
            uint32_t hi = (uint32_t)sat_to(c, in, o + 0x8000, 32) >> 16;
            W(2, (hi << 16) | lo);
        } else {
            o = sat_to(c, in, o, 32);
            uint32_t lo = (uint32_t)sat_to(c, in, (e + 0x4000) << 1, 32) >> 16;
            uint32_t hi = (uint32_t)sat_to(c, in, (o + 0x4000) << 1, 32) >> 16;
            W(2, (hi << 16) | lo);
        }
        return;
    }
    case H_GMPY: W(2, gf_mul((uint32_t)v[0], (uint32_t)v[1], c->cr[CR_GPLYA], 32)); return;
    case H_XORMPY: {
        uint64_t p = 0, a1 = (uint32_t)v[0];
        uint32_t b1 = (uint32_t)v[1];
        for (int k = 0; k < 32; k++) if (b1 & (1u << k)) p ^= a1 << k;
        W(2, (uint32_t)p);
        return;
    }
    case H_GMPY4: {
        uint32_t poly = 0x100 | (c->cr[CR_GFPGFR] & 0xff), o = 0;
        for (int k = 0; k < 32; k += 8)
            o |= gf_mul(((uint32_t)v[0] >> k) & 0xff, ((uint32_t)v[1] >> k) & 0xff, poly, 8) << k;
        W(2, o);
        return;
    }

    case H_ABSSP: W(1, (uint32_t)v[0] & 0x7fffffff); return;
    case H_ABSDP: W(1, v[0] & 0x7fffffffffffffffULL); return;
    case H_ADDSP: case H_SUBSP: case H_MPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float p = u2f((uint32_t)v[0]), q = u2f((uint32_t)v[1]);
        float res = h == H_ADDSP ? p + q : h == H_SUBSP ? p - q : p * q;
        if (rm) fesetround(FE_TONEAREST);
        W(2, f2u(res));
        return;
    }
    case H_ADDDP: case H_SUBDP: case H_MPYDP: case H_MPYSPDP: case H_MPYSP2DP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        double p, q, res;
        if (h == H_MPYSPDP) { p = u2f((uint32_t)v[0]); q = u2d(v[1]); }
        else if (h == H_MPYSP2DP) { p = u2f((uint32_t)v[0]); q = u2f((uint32_t)v[1]); }
        else { p = u2d(v[0]); q = u2d(v[1]); }
        res = h == H_ADDDP ? p + q : h == H_SUBDP ? p - q : p * q;
        if (rm) fesetround(FE_TONEAREST);
        W(2, d2u(res));
        return;
    }
    case H_CMPEQSP: W(2, u2f((uint32_t)v[0]) == u2f((uint32_t)v[1])); return;
    case H_CMPGTSP: W(2, u2f((uint32_t)v[0]) > u2f((uint32_t)v[1])); return;
    case H_CMPLTSP: W(2, u2f((uint32_t)v[0]) < u2f((uint32_t)v[1])); return;
    case H_CMPEQDP: W(2, u2d(v[0]) == u2d(v[1])); return;
    case H_CMPGTDP: W(2, u2d(v[0]) > u2d(v[1])); return;
    case H_CMPLTDP: W(2, u2d(v[0]) < u2d(v[1])); return;
    case H_INTSP: case H_INTSPU: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float f = h == H_INTSP ? (float)(int32_t)v[0] : (float)(uint32_t)v[0];
        if (rm) fesetround(FE_TONEAREST);
        W(1, f2u(f));
        return;
    }
    case H_INTDP: W(1, d2u((double)(int32_t)v[0])); return;
    case H_INTDPU: W(1, d2u((double)(uint32_t)v[0])); return;
    case H_SPINT: W(1, (uint32_t)f_to_i32(u2f((uint32_t)v[0]), 0, rmode(c, in))); return;
    case H_DPINT: W(1, (uint32_t)f_to_i32(u2d(v[0]), 0, rmode(c, in))); return;
    case H_SPTRUNC: W(1, (uint32_t)f_to_i32(u2f((uint32_t)v[0]), 1, 0)); return;
    case H_DPTRUNC: W(1, (uint32_t)f_to_i32(u2d(v[0]), 1, 0)); return;
    case H_SPDP: W(1, d2u((double)u2f((uint32_t)v[0]))); return;
    case H_DPSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float f = (float)u2d(v[0]);
        if (rm) fesetround(FE_TONEAREST);
        W(1, f2u(f));
        return;
    }
    /* The silicon returns an 8-bit-accurate seed for Newton iteration; the
     * exact value converges to the same result. */
    case H_RCPSP: W(1, f2u(1.0f / u2f((uint32_t)v[0]))); return;
    case H_RCPDP: W(1, d2u(1.0 / u2d(v[0]))); return;
    case H_RSQRSP: W(1, f2u(1.0f / sqrtf(u2f((uint32_t)v[0])))); return;
    case H_RSQRDP: W(1, d2u(1.0 / sqrt(u2d(v[0])))); return;

    /* C66x two-way SIMD: the same 32-bit operation on each half of a pair. */
    case H_DADD: case H_DSUB: case H_DSADD: case H_DSSUB: {
        uint32_t p[2], q[2], o[2];
        for (int k = 0; k < 2; k++) {
            p[k] = op[0].kind == C66X_OPK_CONST ? (uint32_t)op[0].val : (uint32_t)(v[0] >> (32 * k));
            q[k] = (uint32_t)(v[1] >> (32 * k));
            int64_t r2 = (h == H_DADD || h == H_DSADD) ? (int64_t)(int32_t)p[k] + (int32_t)q[k]
                                                        : (int64_t)(int32_t)p[k] - (int32_t)q[k];
            if (h == H_DSADD || h == H_DSSUB)
                r2 = sat_to(c, in, r2, 32);
            o[k] = (uint32_t)r2;
        }
        W(2, ((uint64_t)o[1] << 32) | o[0]);
        return;
    }
    case H_DADDSP: case H_DSUBSP: case H_DMPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        uint32_t o[2];
        for (int k = 0; k < 2; k++) {
            float p = u2f((uint32_t)(v[0] >> (32 * k))), q = u2f((uint32_t)(v[1] >> (32 * k)));
            o[k] = f2u(h == H_DADDSP ? p + q : h == H_DSUBSP ? p - q : p * q);
        }
        if (rm) fesetround(FE_TONEAREST);
        W(2, ((uint64_t)o[1] << 32) | o[0]);
        return;
    }
    case H_QMPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        for (int k = 0; k < 4; k++) {
            float p = u2f(c->reg[op[0].reg + k]), q = u2f(c->reg[op[1].reg + k]);
            sched(c, op[2].low_first - 1, WK_REG, op[2].reg + k, f2u(p * q), 0);
        }
        if (rm) fesetround(FE_TONEAREST);
        return;
    }
    case H_CMPYSP: {
        /*
         * Single-precision complex multiply, 128-bit result (SPRUGH7 4.62).
         * Used by the MASTER TEMPO time-stretch. Products in manual order:
         *   dst_0 = src1_lo * src2_hi      dst_1 = -(src1_lo * src2_lo)
         *   dst_2 = src1_hi * src2_lo      dst_3 = src1_hi * src2_hi
         * so dst_1:dst_0 is the real part's two terms (subtract to combine)
         * and dst_3:dst_2 the imaginary part's.
         */
        unsigned rm = rmode(c, in);
        float a_lo = u2f(c->reg[op[0].reg]), a_hi = u2f(c->reg[op[0].reg + 1]);
        float b_lo = u2f(c->reg[op[1].reg]), b_hi = u2f(c->reg[op[1].reg + 1]);
        float r4[4];

        if (rm) fesetround(fe_mode[rm]);
        r4[0] = a_lo * b_hi;
        r4[1] = -(a_lo * b_lo);
        r4[2] = a_hi * b_lo;
        r4[3] = a_hi * b_hi;
        for (int k = 0; k < 4; k++)
            sched(c, op[2].low_first - 1, WK_REG, op[2].reg + k, f2u(r4[k]), 0);
        if (rm) fesetround(FE_TONEAREST);
        return;
    }
    case H_QSMPY32R1:
        for (int k = 0; k < 4; k++) {
            int64_t p = (int32_t)c->reg[op[0].reg + k], q = (int32_t)c->reg[op[1].reg + k];
            int64_t r2 = sat_to(c, in, (p * q + (1LL << 30)) >> 31, 32);
            sched(c, op[2].low_first - 1, WK_REG, op[2].reg + k, (uint32_t)r2, 0);
        }
        return;
    case H_SHL2: {
        /* SPRUGH7 4.261: only the low four bits of the amount count. */
        unsigned n = (uint32_t)v[1] & 0xf;
        uint32_t s = (uint32_t)v[0];
        W(2, (((s >> 16) << n) & 0xffff) << 16 | ((s << n) & 0xffff));
        return;
    }
    case H_DPACKL4: {
        uint64_t o = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t s1 = (uint32_t)(v[0] >> (32 * k)), s2 = (uint32_t)(v[1] >> (32 * k));
            uint32_t w = (((s1 >> 16) & 0xff) << 24) | ((s1 & 0xff) << 16)
                       | (((s2 >> 16) & 0xff) << 8) | (s2 & 0xff);
            o |= (uint64_t)w << (32 * k);
        }
        W(2, o);
        return;
    }
    case H_DCCMPYR1: {
        /*
         * Two CMPYR1s of src1 against the CONJUGATE of src2 (SPRUGH7 4.74).
         * The manual's Execution block has the imaginary term's sign flipped;
         * its seven worked examples (saturating ones included) all agree with
         * src1 * conj(src2), so that is what this computes.
         */
        uint64_t o = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t s1 = (uint32_t)(v[0] >> (32 * k)), s2 = (uint32_t)(v[1] >> (32 * k));
            int64_t im = (int64_t)lsb16s(s1) * msb16s(s2) - (int64_t)msb16s(s1) * lsb16s(s2);
            int64_t re = (int64_t)msb16s(s1) * msb16s(s2) + (int64_t)lsb16s(s1) * lsb16s(s2);
            im = sat_to(c, in, im, 32);
            re = sat_to(c, in, re, 32);
            uint32_t lo = (uint32_t)sat_to(c, in, (im + 0x4000) << 1, 32) >> 16;
            uint32_t hi = (uint32_t)sat_to(c, in, (re + 0x4000) << 1, 32) >> 16;
            o |= (uint64_t)((hi << 16) | lo) << (32 * k);
        }
        W(2, o);
        return;
    }
    case H_MPYU2: {
        uint32_t e = lsb16u(v[0]) * lsb16u(v[1]), o = msb16u(v[0]) * msb16u(v[1]);
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_LAND: W(2, (uint32_t)v[0] != 0 && (uint32_t)v[1] != 0); return;
    case H_LANDN: W(2, (uint32_t)v[0] != 0 && (uint32_t)v[1] == 0); return;
    case H_LOR: W(2, (uint32_t)v[0] != 0 || (uint32_t)v[1] != 0); return;
    case H_DINTHSP: case H_DINTHSPU: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float e = h == H_DINTHSP ? (float)lsb16s(v[0]) : (float)lsb16u(v[0]);
        float o = h == H_DINTHSP ? (float)msb16s(v[0]) : (float)msb16u(v[0]);
        if (rm) fesetround(FE_TONEAREST);
        W(1, ((uint64_t)f2u(o) << 32) | f2u(e));
        return;
    }
    case H_DINTSP: case H_DINTSPU: {
        /* SPRUGH7 4.93 (printed under the heading DINTHSP) and 4.95: each
         * 32-bit word of the src2 pair to single precision. */
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        uint32_t w_e = (uint32_t)v[0], w_o = (uint32_t)(v[0] >> 32);
        float e = h == H_DINTSP ? (float)(int32_t)w_e : (float)w_e;
        float o = h == H_DINTSP ? (float)(int32_t)w_o : (float)w_o;
        if (rm) fesetround(FE_TONEAREST);
        W(1, ((uint64_t)f2u(o) << 32) | f2u(e));
        return;
    }
    case H_DSPINT: {
        uint32_t e = (uint32_t)f_to_i32(u2f((uint32_t)v[0]), 0, rmode(c, in));
        uint32_t o = (uint32_t)f_to_i32(u2f((uint32_t)(v[0] >> 32)), 0, rmode(c, in));
        W(1, ((uint64_t)o << 32) | e);
        return;
    }
    case H_DSADD2: case H_DAVGNR2: case H_DCMPGT2: case H_DSHL2: case H_UNPKH2: case H_DMPY2: {
        uint64_t res = 0;
        for (int k = 0; k < 4; k++) {
            int32_t p = (int16_t)(v[0] >> (16 * k)), q = (int16_t)(v[1] >> (16 * k));
            int32_t t;
            switch (h) {
            case H_DSADD2: t = sat16(p + q); break;
            case H_DAVGNR2: t = (p + q) >> 1; break;
            case H_DCMPGT2: t = p > q; break;
            default: t = 0; break;
            }
            if (h == H_DCMPGT2)
                res |= (uint64_t)t << k;
            else if (h != H_UNPKH2 && h != H_DMPY2)
                res |= (uint64_t)(uint16_t)t << (16 * k);
        }
        if (h == H_UNPKH2)
            res = ((uint64_t)(uint32_t)msb16s(v[0]) << 32) | (uint32_t)lsb16s(v[0]);
        /* One site each in the image; left as traps until one executes. */
        if (h == H_DMPY2 || h == H_DSHL2) {
            c->trap_pc = in->addr;
            x->stop = C66X_STOP_UNDEF;
            return;
        }
        W(LAST, res);
        return;
    }

    case H_LOAD:
        do_load(c, in);
        return;
    case H_STORE:
        do_store(c, in, x->pce1);
        return;

    case H_B:
        if (op[0].kind == C66X_OPK_IRP) {
            uint32_t itsr = c->cr[CR_ITSR];
            c->cr[CR_TSR] = itsr;
            take_branch(c, c->cr[CR_IRP], x);
            idle_isr_return(c, c->cr[CR_IRP]);
        } else if (op[0].kind == C66X_OPK_NRP) {
            c->cr[CR_TSR] = c->cr[CR_NTSR];
            c->cr[CR_IER] |= IER_NMIE;
            take_branch(c, c->cr[CR_NRP], x);
            idle_isr_return(c, c->cr[CR_NRP]);
        } else {
            take_branch(c, (uint32_t)opval(c, &op[0], x->pce1), x);
        }
        return;
    case H_BNOP:
        take_branch(c, (uint32_t)opval(c, &op[0], x->pce1), x);
        return;
    case H_CALLP:
        take_branch(c, (uint32_t)op[0].val, x);
        W(1, x->next_pc);
        return;
    case H_BDEC:
        if ((int32_t)v[1] >= 0) {
            take_branch(c, (uint32_t)op[0].val, x);
            W(1, (uint32_t)v[1] - 1);
        }
        return;
    case H_BPOS:
        if ((int32_t)v[1] >= 0)
            take_branch(c, (uint32_t)op[0].val, x);
        return;

    case H_SPLOOP: case H_SPLOOPD: case H_SPLOOPW:
        spl_start(c, in, x);
        return;
    case H_SPKERNEL: case H_SPMASK:
        return;           /* handled at packet level */
    case H_SPKERNELR: case H_SPMASKR:
        c->trap_pc = in->addr;
        x->stop = C66X_STOP_UNDEF;   /* loop reload: not used by this image */
        return;
    }
    c->trap_pc = in->addr;
    x->stop = C66X_STOP_UNDEF;
#undef SV
#undef UV
#undef W
#undef LAST
}

/* ------------------------------------------------------------------------ */
/* SPLOOP loop buffer (SPRU732 chapter 7)                                    */

static void spl_start(c66x_core *c, const c66x_insn *in, xctx *x)
{
    spl_state *s = &c->spl;
    int resumed = (c->cr[CR_TSR] & TSR_SPLX) != 0;
    memset(s, 0, sizeof *s);
    s->active = 1;
    s->kind = in->handler;
    s->ii = in->op[0].val;
    s->creg = in->cond_reg;
    s->cz = in->cond_z;
    s->addr = in->addr;
    s->t0 = c->cycle + 1;
    s->loading = 1;
    s->resumed = resumed;
    if (resumed && s->kind == H_SPLOOPD)
        s->kind = H_SPLOOP;
    c->cr[CR_TSR] |= TSR_SPLX;
    c->st.sploops++;
    if (s->kind == H_SPLOOP) {
        if (c->cr[CR_ILC] == 0) {
            s->initial_term = 1;
            s->terminated = 1;
            s->drain_start = s->t0;
        } else {
            c->cr[CR_ILC]--;
        }
    }
}

static int spl_cond_true(c66x_core *c, uint64_t cyc)
{
    uint32_t r = c->cond_hist[cyc & 7];
    return (r == 0) == c->spl.cz;
}

/* The loop stopped starting iterations at drain_start: when the kernel had
 * disabled program fetch, post-loop code waits for the fetch delay (capped at
 * the epilog, SPKERNEL note) plus the pipeline refill. */
static void spl_schedule_resume(c66x_core *c, int delay)
{
    spl_state *s = &c->spl;
    if (s->loading)
        return;                         /* early exit: fetch was never disabled */
    int epilog = s->dynlen - s->ii;
    if (epilog < 0)
        epilog = 0;
    if (delay > epilog)
        delay = epilog;
    uint64_t enable = s->drain_start + delay;
    if (enable > s->kernel_cycle + 1)
        c->pm_resume_at = enable + PM_REFILL;
}


/* r / ii and r % ii for r = cycle - t0 (cycle >= t0). */
static inline void spl_pos(c66x_core *c, uint64_t *k, uint32_t *off)
{
    spl_state *s = &c->spl;
    uint32_t ii = (uint32_t)s->ii;
    if (s->pos_t0 == s->t0 && s->pos_ii == ii && s->pos_cycle + 1 == c->cycle) {
        if (++s->pos_off == ii) {
            s->pos_off = 0;
            s->pos_k++;
        }
    } else if (!(s->pos_t0 == s->t0 && s->pos_ii == ii && s->pos_cycle == c->cycle)) {
        uint64_t r = c->cycle - s->t0;
        s->pos_k = r / ii;
        s->pos_off = (uint32_t)(r - s->pos_k * ii);
        s->pos_t0 = s->t0;
        s->pos_ii = ii;
    }
    s->pos_cycle = c->cycle;
    *k = s->pos_k;
    *off = s->pos_off;
}

/* Instructions the loop buffer issues this cycle, iterations k >= 1. */
unsigned spl_buffer_insns(c66x_core *c, c66x_insn **out, uint32_t mask)
{
    spl_state *s = &c->spl;
    unsigned n = 0;
    if (!s->active || s->initial_term || c->cycle < s->t0)
        return 0;
    int64_t r = c->cycle - s->t0;
    int loaded = s->loading ? (int)r : s->dynlen;
    /* Only the iterations whose body offset falls in [0, loaded) this cycle;
     * a long SPLOOPW runs millions of iterations. */
    uint64_t kq;
    uint32_t oq;
    spl_pos(c, &kq, &oq);
    int64_t kmax = (int64_t)kq;
    if (kmax > s->last_iter) kmax = s->last_iter;
    int64_t kmin = kmax + 1;
    while (kmin > 1 && r - (kmin - 1) * s->ii < loaded)
        kmin--;
    for (int64_t k = kmin; k <= kmax; k++) {
        int64_t off = r - (int64_t)k * s->ii;
        spl_ent *e = &s->body[off];
        for (unsigned i = 0; i < e->n && n < MAX_PK; i++) {
            c66x_insn *in = e->insn[i];
            if (!s->resumed && in->unit >= 0 && (mask & (1u << in->unit)))
                continue;
            out[n++] = in;
        }
    }
    return n;
}

/* End of a cycle: record the program-memory packet, then the stage boundary. */
void spl_end_cycle(c66x_core *c, c66x_insn **pm, unsigned npm, uint32_t mask, int *pm_exec_mask)
{
    spl_state *s = &c->spl;
    if (!s->active || c->cycle < s->t0)
        return;
    int64_t r = c->cycle - s->t0;

    if (s->loading && r < 48) {
        spl_ent *e = &s->body[r];
        e->n = 0;
        int kernel = 0, fstg = 0, fcyc = 0;
        for (unsigned i = 0; i < npm; i++) {
            c66x_insn *in = pm[i];
            if (in->handler == H_SPKERNEL) {
                kernel = 1;
                if (in->nops >= 2) { fstg = in->op[0].val; fcyc = in->op[1].val; }
                continue;
            }
            if (in->handler == H_SPMASK || in->handler == H_BNOP || in->handler == H_NOP)
                continue;
            if (in->unit >= 0 && (mask & (1u << in->unit)))
                continue;
            if (e->n < 8)
                e->insn[e->n++] = in;
        }
        if (kernel) {
            s->loading = 0;
            s->dynlen = r + 1;
            s->fetch_delay = fstg * s->ii + fcyc;
            s->kernel_cycle = c->cycle;
        }
    }
    (void)pm_exec_mask;

    uint64_t pk;
    uint32_t poff;
    spl_pos(c, &pk, &poff);
    if (poff + 1 == (uint32_t)s->ii && !s->terminated && !s->abrupt) {
        int early3 = (s->kind != H_SPLOOP) && (r + 1) <= 3;
        int term = s->kind == H_SPLOOPW ? (!early3 && !spl_cond_true(c, c->cycle - 3))
                                        : (!early3 && c->cr[CR_ILC] == 0);
        /* SPRU732 7.13.1: an interrupt drains the loop at a stage boundary. */
        if (!term && !early3 && !s->loading && c->ifr && pending_interrupt(c)
            && !c->nbr && !c->branch_block
            && (s->kind == H_SPLOOPW
                || c->cr[CR_ILC] >= (uint32_t)((s->dynlen + s->ii - 1) / s->ii))) {
            s->int_drain = 1;
            s->terminated = 1;
            s->drain_start = c->cycle + 1;
        } else if (s->kind == H_SPLOOPW) {
            if (!early3 && !spl_cond_true(c, c->cycle - 3)) {
                s->abrupt = 1;
                s->terminated = 1;
                s->drain_start = c->cycle + 1;
                spl_schedule_resume(c, 0);
            } else {
                s->last_iter = (r + 1) / s->ii;
            }
        } else if (early3) {
            s->last_iter = (r + 1) / s->ii;
        } else if (c->cr[CR_ILC] == 0) {
            s->terminated = 1;
            s->drain_start = c->cycle + 1;
            spl_schedule_resume(c, s->fetch_delay);
        } else {
            c->cr[CR_ILC]--;
            s->last_iter = (r + 1) / s->ii;
        }
    }

    /* Idle once nothing is loading and the last started iteration has finished. */
    if (!s->loading) {
        int done;
        if (s->abrupt)
            done = 1;
        else if (s->terminated) {
            int64_t end = (int64_t)s->last_iter * s->ii + s->dynlen;
            if (s->initial_term)
                end = ((s->dynlen + s->ii - 1) / s->ii) * s->ii;
            done = r + 1 >= end;
        } else
            done = 0;
        if (done) {
            HCLOOP(s->addr, (uint64_t)s->last_iter + 1, r + 1);
            s->active = 0;
            c->cr[CR_TSR] &= ~TSR_SPLX;
            if (s->int_drain) {
                c->spl_irq_pending = 1;
                c->spl_irq_ret = s->addr;
            }
        }
    }
}
