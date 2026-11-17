/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: the NXS2 playback chain. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* polyphase FIR, one output per pass (NXS2: 0x800169AC)                    */

/* fir(in A4, out B4, count A6, channel B6). Per output: phase p = *++B16,
 * input offset k = *++A18 and h = the halfword at A19 + 4. While p <= h >> 1
 * the output is a 29-tap dot product of in[k..k+28] with the coefficient row
 * B9 + 116 p, summed in one serial faddsp chain starting from A20. Otherwise
 * a SPLOOP (14 iterations from ILC 12) sums in[k..k+27] against the row
 * R = B9 + 116 (h - p) read backwards, in two interleaved partial sums, and
 * the centre tap in[k+28] takes its coefficient from the channel's table at
 * *(addr[0] + 4 ch), addr[0] being 0x0088B2E0 on the NXS2. The pass costs
 * and counters below were measured on the interpreter and hold for every
 * pass of both paths. */

static int fir_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (((s->cr[CR_FADCR] | s->cr[CR_FMCR]) & (3u << 9 | 3u << 25))
        || r[A(27)] != 104 || r[B(27)] != 104 || r[A(28)] != 88 || r[B(28)] != 88
        || r[A(29)] != 72 || r[B(29)] != 72 || r[B(30)] != 56)
        return 0;
    const uint8_t *pp = ram_ptr(c, r[B(16)] + 4, 4), *kp = ram_ptr(c, r[A(18)] + 4, 4);
    const uint8_t *hp = ram_ptr(c, r[A(19)] + 4, 2);
    if (!pp || !kp || !hp || !ram_ptr(c, r[A(17)], 4))
        return 0;
    uint32_t p = ld32(pp), k = ld32(kp);
    int32_t h = (int16_t)(hp[0] | hp[1] << 8);
    uint32_t in = r[A(16)] + 4 * k;
    float acc;

    if (!((int32_t)p > h >> 1)) {
        uint32_t row = r[B(9)] + (uint32_t)(116 * (int64_t)(int32_t)p);
        const uint8_t *xp = ram_ptr(c, in, 116), *cp = ram_ptr(c, row, 116);
        if (!xp || !cp)
            return 0;
        float x[29], cf[29], pr[29];
        memcpy(x, xp, sizeof x);
        memcpy(cf, cp, sizeof cf);
        for (int i = 0; i < 29; i++)
            pr[i] = x[i] * cf[i];
        acc = u2f(r[A(20)]) + pr[0];
        for (int i = 1; i < 25; i++)
            acc = pr[i] + acc;
        float s25 = pr[25] + acc, s26 = pr[26] + s25, s27 = pr[27] + s26;
        acc = pr[28] + s27;
        r[A(4)] = f2u(s27);
        r[A(5)] = f2u(pr[27]);
        r[A(6)] = f2u(s25);
        r[A(7)] = in + 112;
        r[A(8)] = f2u(pr[24]);
        r[A(9)] = f2u(pr[25]);
        r[A(22)] = f2u(pr[22]);
        r[A(23)] = f2u(pr[23]);
        r[A(24)] = f2u(pr[12]);
        r[A(25)] = f2u(pr[13]);
        r[A(26)] = f2u(x[28]);
        r[A(30)] = 56;
        r[A(31)] = 32;
        r[B(1)] = 0;
        r[B(4)] = 96;
        r[B(5)] = row;
        r[B(6)] = f2u(cf[26]);
        r[B(7)] = f2u(cf[27]);
        r[B(26)] = f2u(cf[28]);
        r[B(31)] = 32;
        s->cycles = 131;
        s->packets = 105;
        s->insns = 128;
    } else {
        uint32_t rrow = r[B(9)] - (uint32_t)(116 * (int64_t)(int32_t)p) + (uint32_t)(116 * h);
        const uint8_t *xp = ram_ptr(c, in, 116), *cp = ram_ptr(c, rrow, 112);
        const uint8_t *tp = ram_ptr(c, s->addr[0] + r[B(17)], 4);
        if (!xp || !cp || !tp)
            return 0;
        uint32_t tab = ld32(tp) + 4 * p - ((2 * (uint32_t)h) & ~3u) - 4;
        const uint8_t *ctr = ram_ptr(c, tab, 4);
        if (!ctr)
            return 0;
        float x[29], cf[28];
        memcpy(x, xp, sizeof x);
        memcpy(cf, cp, sizeof cf);
        float sa = u2f(r[A(20)]), sb = sa;
        for (int j = 0; j < 14; j++) {
            sa = cf[26 - 2 * j] * x[2 * j + 1] + sa;
            sb = cf[27 - 2 * j] * x[2 * j] + sb;
        }
        float cc = u2f(ld32(ctr));
        acc = x[28] * cc + (sa + sb);
        r[A(4)] = f2u(x[26]);
        r[A(5)] = f2u(x[27]);
        r[A(6)] = f2u(sa);
        r[A(7)] = in + 112;
        r[A(8)] = rrow - 8;
        r[A(9)] = (uint32_t)h;
        r[A(30)] = f2u(sa + sb);
        r[A(31)] = f2u(x[28]);
        r[B(1)] = 1;
        r[B(4)] = f2u(cc);
        r[B(5)] = f2u(sb);
        r[B(6)] = rrow - 4;
        r[B(7)] = p;
        r[B(8)] = ld32(tp);
        r[B(31)] = 12;
        s->cr[CR_ILC] = 0;
        s->sploops = 1;
        s->cycles = 111;
        s->packets = 43;
        s->insns = 149;
        s->pm_resume = 84;
    }
    r[A(3)] = f2u(acc);
    r[B(16)] += 4;
    r[A(18)] += 4;
    put_st(s, r[A(17)], f2u(acc), s->at + 0x2B0);
    r[A(17)] += 4;
    r[B(0)] -= 1;
    s->pc = s->at + (r[B(0)] ? 0x18 : 0x2BC);
    s->last_pc = s->at + 0x2B8;
    return 1;
}

/* the MVK/MVKH pair of the centre-tap table */
static const native_wild fir_wild[] = { ADDR_LO(0x210, 0), ADDR_HI(0x21C, 0) };

const native_kernel native_fir = KERNEL("fir", 0x0e802428, 0x2C0, 0x53818cd17afe5d79ull, 0x18, 0x2BC, fir_wild, fir_pass);

/* ------------------------------------------------------------------------ */
/* cubic interpolator, one call per pass (NXS2: 0x80010BD8)                 */

/* interp(x A4, samples B4 A6 B6 A8), registers only: the Lagrange cubic
 * through four samples at fraction x, with the constants 2.0, 0.5 and 1/6
 * the code builds itself. The op order and every register it leaves are the
 * code's; 40 cycles a call, measured on the interpreter. */

static int interp_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (directed_rounding(s))
        return 0;
    float x = u2f(r[A(4)]), s0 = u2f(r[B(4)]), s1 = u2f(r[A(6)]), s2 = u2f(r[B(6)]), s3 = u2f(r[A(8)]);
    float x2 = x * x;
    float x3 = x * x2;
    float a9 = x2 + x2;
    float b5 = s0 * x2;
    float a7 = x3 - x;
    float a4 = x + x;
    float a17 = x3 - x2;
    a9 = a7 - a9;
    float a5 = a4 + x3;
    a17 = a17 - a4;
    a9 = 2.0f + a9;
    float a3 = s3 * a7;
    float a16 = a17 * s2;
    a4 = s1 * a9;
    float b6 = s0 * a5;
    a4 = a4 - a16;
    b5 = b5 + a4;
    a4 = a3 - b6;
    float b4 = 0.5f * b5;
    a3 = u2f(0x3E2AAAAB) * a4;
    a4 = a3 + b4;
    r[A(3)] = f2u(a3);
    r[A(4)] = f2u(a4);
    r[A(5)] = f2u(a5);
    r[A(7)] = f2u(a7);
    r[A(9)] = f2u(a9);
    r[A(16)] = f2u(a16);
    r[A(17)] = f2u(a17);
    r[B(4)] = f2u(b4);
    r[B(5)] = f2u(b5);
    r[B(6)] = f2u(b6);
    s->pc = r[B(3)];
    s->last_pc = s->at + 0x98;
    s->cycles = 40;
    s->packets = 31;
    s->insns = 32;
    return 1;
}

static const native_wild interp_wild[] = { };

const native_kernel native_interp = KERNEL("interp", 0x00006000, 0xA0, 0x131da3c257dffec2ull, 0x18, RETURNS, interp_wild, interp_pass);

/* ------------------------------------------------------------------------ */
/* two-pole filter step with a +-2.0 clip, one call per pass (NXS2: 0x80010C5C) */

/* step(A4 A6 A8 A10 A12 B4 B6 B8 B10 state and coefficient pointers): two
 * cascaded sections, each x - a * y[0] scaled by b and stored, then checked
 * against +-2.0. The clip branches are left to the interpreter. A12 is saved
 * on the stack across the call. One store may land where a later load reads:
 * then too the interpreter runs the call. 84 cycles, measured on the
 * interpreter. */

static int filter_step_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (directed_rounding(s))
        return 0;
    uint32_t a12 = r[A(12)], b15 = r[B(15)], b6 = r[B(6)], b10 = r[B(10)];
    const uint8_t *y0p = ram_ptr(c, a12, 8), *ap = ram_ptr(c, r[A(6)], 4), *xp = ram_ptr(c, r[A(4)], 4);
    const uint8_t *bp = ram_ptr(c, r[B(4)], 8), *a2p = ram_ptr(c, r[A(10)], 4), *x2p = ram_ptr(c, r[A(8)], 4);
    const uint8_t *b2p = ram_ptr(c, r[B(8)], 8);
    if (!y0p || !ap || !xp || !bp || !a2p || !x2p || !b2p || !ram_ptr(c, b15, 4) || !ram_ptr(c, b6, 4)
        || !ram_ptr(c, b10, 4))
        return 0;
    /* every load after the stack store, the second section's after the
     * first section's stores, and the reload of A12 after all of them */
    const uint32_t first[] = { a12, r[A(6)], r[A(4)], r[B(4)], r[B(4)] + 4 };
    const uint32_t second[] = { a12 + 4, r[A(10)], r[A(8)], r[B(8)], r[B(8)] + 4 };
    for (unsigned i = 0; i < 5; i++)
        if (same_word(first[i], b15) || same_word(second[i], b15) || same_word(second[i], b6)
            || same_word(second[i], a12))
            return 0;
    if (same_word(b15, b6) || same_word(b15, a12) || same_word(b15, a12 + 4) || same_word(b15, b10))
        return 0;

    uint32_t a7l = ld32_staged(s, a12, y0p);
    float a7 = u2f(a7l), a3 = a7 * u2f(ld32_staged(s, r[A(6)], ap));
    float a5 = u2f(ld32_staged(s, r[A(4)], xp)) - a3;
    uint32_t b9l = ld32_staged(s, r[B(4)] + 4, bp + 4);
    float b4 = u2f(ld32_staged(s, r[B(4)], bp)) * a5;
    float b7 = u2f(b9l) * a7;
    b4 = b7 + b4;
    if (a5 > 2.0f || a5 < -2.0f)
        return 0;
    uint32_t a9l = ld32_staged(s, a12 + 4, y0p + 4);
    float a9 = u2f(a9l);
    float m = a9 * u2f(ld32_staged(s, r[A(10)], a2p));
    float y = u2f(ld32_staged(s, r[A(8)], x2p)) - m;
    float coef0 = u2f(ld32_staged(s, r[B(8)], b2p));
    float b5 = coef0 * y;
    float b4b = coef0 * a9;
    b4b = b4b + b5;
    if (y < -2.0f || y > 2.0f)
        return 0;

    put_st(s, b15, a12, s->at + 0x1C);
    put_st(s, b6, f2u(b4), s->at + 0x60);
    put_st(s, a12, f2u(a5), s->at + 0xD0);
    put_st(s, b10, f2u(b4b), s->at + 0x114);
    put_st(s, a12 + 4, f2u(y), s->at + 0x16C);
    r[A(0)] = 0;
    r[A(3)] = 0x40000000;
    r[A(4)] = 0xC0000000;
    r[A(5)] = 0;
    r[A(6)] = 0;
    r[A(7)] = f2u(y);
    r[A(9)] = a9l;
    r[B(4)] = f2u(b4b);
    r[B(5)] = f2u(b5);
    r[B(7)] = f2u(b7);
    r[B(8)] += 4;
    r[B(9)] = b9l;
    s->pc = r[B(3)];
    s->last_pc = s->at + 0x172;
    s->cycles = 88;
    s->packets = 49;
    s->insns = 52;
    return 1;
}

static const native_wild filter_step_wild[] = { };

const native_kernel native_filter_step = KERNEL("filter_step", 0x01d555a8, 0x180, 0xcaea3cdb3d032fdaull, 0x1C, RETURNS, filter_step_wild, filter_step_pass);
